package com.mapswithme.maps.editor;

import android.app.Activity;
import android.content.Intent;
import android.support.annotation.NonNull;
import android.support.annotation.StyleRes;
import android.support.v4.app.Fragment;

import com.mapswithme.maps.R;
import com.mapswithme.maps.base.BaseMwmFragmentActivity;
import com.mapswithme.util.ThemeUtils;

public class EditorActivity extends BaseMwmFragmentActivity
{
  public static final String EXTRA_NEW_OBJECT = "ExtraNewMapObject";

  @Override
  protected Class<? extends Fragment> getFragmentClass()
  {
    return EditorHostFragment.class;
  }

  public static void start(@NonNull Activity activity)
  {
    final Intent intent = new Intent(activity, EditorActivity.class);
    activity.startActivity(intent);
  }
}
